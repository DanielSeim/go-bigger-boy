#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned direct-return geometry and bounded opaque held-voice observations."""
import copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_direct_return_fixture import bank,build,CASES,DURATIONS,ARTICULATIONS
from check_sgb_score_direct_return_reference import expected,observe,contract,agree
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule


def trace(case,art,duration,*,release_old=False):
    rows=[(0,61,0)]
    for i,e in enumerate(expected(case)):
        c=10000+(i*16 if i<5 else 64+duration)*5500
        for v in e['voices']:
            if i==5:
                rows.extend(((c-500,34,8),(c-464,35,7)))
            elif i==4:
                rows.extend(((c-1479,34,164),(c-1443,35,6),(c-651,32,7),(c-509,33,7)))
            else:
                values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
                rows.extend((c-700+20*r,16*v['voice']+r,value) for r,value in enumerate(values))
            if i<5 and (i,v['voice'])!=(1,2):
                pulse=PULSES[(96,art,duration)] if i==4 else PULSES[(96,127,16)]
                rows.append((c+pulse*2048,92,1<<v['voice']))
        if i==4:
            rows.extend(((c-156,92,4 if release_old else 0),(c-39,92,0)))
        if i!=5:rows.append((c,76,e['mask']))
    stop=10000+(64+duration)*5500-156
    rows.extend(((stop,92,255),(stop+117,92,0),(stop+156,76,4 if case=='direct-note' else 0),(stop+1000,76,0),(stop+400000,61,0)))
    rows.sort(key=lambda row:row[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'+''.join(f'D,0,{c},0,{a},{v}\n' for c,a,v in rows)


def references():
    refs=[]
    for c in CASES:
        for a in ARTICULATIONS:
            for d in DURATIONS:
                r=observe(io.StringIO(trace(c,a,d)),c,a,d)
                refs.extend({**copy.deepcopy(r),'case':c,'model':m,'fixture_sha256':hashlib.sha256(build(a,d,c)).hexdigest()} for m in ('sgb','sgb2'))
    return refs


class DirectReturnTests(unittest.TestCase):
    def test_owned_reproducible_cartridges_and_input_bounds(self):
        hashes=set()
        for c in CASES:
            for a in ARTICULATIONS:
                for d in DURATIONS:
                    self.assertEqual(len(bank(a,d,c)),2048)
                    self.assertEqual(build(a,d,c),build(a,d,c))
                    hashes.add(hashlib.sha256(build(a,d,c)).hexdigest())
        self.assertEqual(len(hashes),8)
        for args in ((True,8,'direct-note'),(127,True,'direct-note'),(31,8,'direct-note'),(63,4,'direct-note'),(63,8,'unknown')):
            with self.assertRaises(ValueError):bank(*args)

    def test_symbolic_clipping_direct_note_and_terminal_readiness(self):
        for c in CASES:
            for a in ARTICULATIONS:
                for d in DURATIONS:
                    b=bank(a,d,c);s=schedule(b,int.from_bytes(b[:2],'little'),inherit_timing=True,end_priority=True,boundary_events=True)
                    self.assertEqual(s['ticks'],64+d)
                    self.assertEqual([p['start_tick'] for p in s['patterns']],[0,32,64])
                    self.assertEqual([p['channels'] for p in s['patterns']],[[2,3],[3],[2,3]])
                    self.assertEqual(s['patterns'][0]['truncated_channels'],[2])
                    es=[e for e in s['events'] if e['kind'] in ('note','rest')]
                    self.assertEqual(len(es),9);self.assertEqual([e['articulation'] for e in es],[127]*6+[a]*3)
                    self.assertEqual((es[6]['tick'],es[6]['kind'],es[6]['note']),(64,'note',32))
                    self.assertEqual((es[7]['tick'],es[7]['kind']),(64,'rest'))
                    self.assertEqual((es[-1]['tick'],es[-1]['kind'],es[-1]['boundary_event']),(64+d,'note' if c=='direct-note' else 'rest',True))

    def test_complete_synthetic_two_model_matrix(self):
        refs=references();self.assertEqual(len(agree(refs)),8)
        for r in refs:
            self.assertEqual(r['unreleased_retriggers'][0]['previous_onset_index'],1)
            self.assertEqual(r['unreleased_retriggers'][0]['onset_index'],4)
            self.assertNotIn((1,2),[(g['onset_index'],g['voice']) for g in r['note_gates']])
            self.assertEqual(r['voices_pending_at_stop'],[])
            self.assertEqual(r['unreleased_final_voices'],[2] if r['case']=='direct-note' else [])

    def test_inserting_a_pre_return_release_breaks_the_contract(self):
        with self.assertRaises(ValueError):observe(io.StringIO(trace('direct-note',63,8,release_old=True)),'direct-note',63,8)

    def test_retrigger_and_raw_interval_forgery_rejected(self):
        for field,value in (('voice',3),('previous_onset_index',0),('onset_index',5),('interval_spc_cycles',1)):
            r=copy.deepcopy(references()[0]);r['unreleased_retriggers'][0][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):contract(r,r['case'])
        r=references()[0];r['unreleased_retriggers']=[]
        with self.assertRaises(ValueError):contract(r,r['case'])

    def test_release_source_pulse_pitch_volume_and_tail_forgeries_rejected(self):
        for mutation in ('source','gate','return-kof','return-pitch','stop-held','final-kof','final-pitch','final-volume','tail','types'):
            r=copy.deepcopy(references()[0])
            if mutation=='source':r['note_gates'][-1]['release_kind']='final-stop'
            elif mutation=='gate':r['note_gates'][-1]['gate_spc_cycles']=1
            elif mutation=='return-kof':r['return_control_writes'][0]['value']=4
            elif mutation=='return-pitch':r['return_voice_writes'][1]['value']=7
            elif mutation=='stop-held':r['voices_pending_at_stop']=[2]
            elif mutation=='final-kof':r['final_control_writes'][0]['value']=4
            elif mutation=='final-pitch':r['boundary_pitch_writes'][0]['pitch']=1700
            elif mutation=='final-volume':r['post_return_volume_writes']=[{'address':32,'value':1}]
            elif mutation=='tail':r['post_stop_observation_spc_cycles']=1
            elif mutation=='types':r['return_duration']=True
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):contract(r,r['case'])

    def test_missing_duplicate_model_hash_and_intermodel_drift_rejected(self):
        refs=references()
        with self.assertRaises(ValueError):agree(refs[:-1])
        with self.assertRaises(ValueError):agree(refs[:-1]+[refs[0]])
        for field,value in (('model','unknown'),('fixture_sha256','0'*64)):
            changed=copy.deepcopy(refs);changed[0][field]=value
            with self.assertRaises(ValueError):agree(changed)
        changed=copy.deepcopy(refs);changed[1]['note_gates'][0]['gate_spc_cycles']+=3000
        with self.assertRaisesRegex(ValueError,'allowance'):agree(changed)

    def test_trace_header_byte_row_order_and_integer_bounds(self):
        for text in ('bad header\n','x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'direct-note')
        header='kind,master_clock,spc_cycle,pcm_sample,address,value\n'
        with self.assertRaisesRegex(ValueError,'row bound'):observe(io.StringIO(header+'D,0,0,0,61,0\n'*32768),'direct-note')
        for rows in ('D,0,2,0,61,0\nD,0,1,0,61,0\n','D,0,0,0,128,0\n','D,0,0,0,61,256\n','D,0,0,0,61,1.0\n'):
            with self.assertRaises(ValueError):observe(io.StringIO(header+rows),'direct-note')


if __name__=='__main__':unittest.main()
