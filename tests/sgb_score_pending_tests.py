#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opaque observation contracts and continued silent native rejection."""
import argparse,copy,hashlib,io,json
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_pending_fixture import bank,build,CASES,DURATIONS
from check_sgb_score_pending_reference import expected,ticks,observe,contract,agree,native_rejections,rejection
from check_sgb_score_reverse_reference import native
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule
from build_sgb_score_reverse import build as program_build
PROBE=None


def trace(case,art,duration):
    events=[]
    for i,(edge,tick) in enumerate(zip(expected(case),ticks(case))):
        cycle=10000+tick*5500
        for v in edge['voices']:
            values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
            events.extend((cycle,16*v['voice']+r,value) for r,value in enumerate(values))
            if i==2 and v['voice']==2:
                if case=='note-return':continue
                gate=176000+PULSES[(96,art,16 if case=='long-rest-return' else 8)]*2048
            else:gate=PULSES[(96,art,16)]*2048
            events.append((cycle+gate,0x5C,1<<v['voice']))
        events.append((cycle,0x4C,edge['mask']))
    events.sort(key=lambda e:e[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\nD,0,0,0,61,0\n'+''.join(f'D,0,{c},0,{a},{v}\n' for c,a,v in events)


def references():
    result=[]
    for c in CASES:
        for d in DURATIONS:
            for a in (63,127):
                r=observe(io.StringIO(trace(c,a,d)),c,a,d)
                result.extend({**copy.deepcopy(r),'case':c,'model':m} for m in ('sgb','sgb2'))
    return result


class PendingTests(unittest.TestCase):
    def test_owned_fixture_layout_and_boundary_oracle(self):
        hashes=set()
        for c in CASES:
            for d in DURATIONS:
                for a in (63,127):
                    data=bank(a,d,c)
                    self.assertEqual(len(data),2048)
                    self.assertEqual(int.from_bytes(data[0x3FB+4:0x3FB+6],'little'),0)
                    self.assertEqual(int.from_bytes(data[0x3FB+6:0x3FB+8],'little'),0x31FD)
                    symbolic=schedule(data,int.from_bytes(data[:2],'little'),inherit_timing=True,end_priority=True,boundary_events=True)
                    self.assertEqual([p['channels'] for p in symbolic['patterns']],[[2,3],[3],[2],[2,3]])
                    self.assertEqual([p['start_tick'] for p in symbolic['patterns']],[0,32,64,112 if c=='long-rest-return' else 104 if c=='rest-return' else 96])
                    boundary=[e for e in symbolic['events'] if e.get('boundary_event')]
                    self.assertEqual(len(boundary),1)
                    self.assertEqual((boundary[0]['tick'],boundary[0]['channel'],boundary[0]['duration'],boundary[0]['articulation']),(32,2,d,a))
                    self.assertEqual(boundary[0]['kind'],'rest' if c=='pending-rest' else 'note')
                    image=build(a,d,c);self.assertEqual(image,build(a,d,c));hashes.add(hashlib.sha256(image).hexdigest())
        self.assertEqual(len(hashes),16)
        self.assertEqual(hashlib.sha256(program_build()).hexdigest(),'deece9488c7138fb4e9f0cde22beccf2edd3770f42b1df2a257f9b387cd15dda')

    def test_all_pending_banks_remain_silently_rejected(self):
        results=native_rejections(PROBE);self.assertEqual(len(results),16)
        for r in results.values():
            self.assertEqual(r['event_patterns'],[]);self.assertEqual(r['pattern_ticks'],[])
            self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal'])
            self.assertEqual(r['frozen_peer_checks'],[0,0])

    def test_pending_note_joins_next_pattern_KON(self):
        for a in (63,127):
            r=observe(io.StringIO(trace('note-return',a,8)),'note-return',a,8)
            self.assertEqual(r['keyons'][2]['mask'],12)
            self.assertEqual(r['keyons'][2]['voices'][0]['pitch'],1700)
            self.assertEqual(r['keyons'][2]['voices'][0]['volumes'],[7,7])
            self.assertEqual(r['voice2_volume_updates'][0]['volumes'],[1,1])
            self.assertEqual(r['unreleased_retriggers'][0]['previous_onset_index'],2)
            self.assertEqual(len(r['note_gates']),12)
            self.assertFalse(any(g['voice']==2 and g['onset_index']==2 for g in r['note_gates']))

    def test_returning_rest_controls_release_independently_of_pending_duration(self):
        for c in ('rest-return','long-rest-return'):
            for a in (63,127):
                gates=[]
                for d in DURATIONS:
                    r=observe(io.StringIO(trace(c,a,d)),c,a,d)
                    held=[g for g in r['note_gates'] if g['voice']==2 and g['onset_index']==2]
                    self.assertEqual(len(held),1);self.assertGreater(held[0]['gate_spc_cycles'],190000)
                    self.assertEqual(r['unreleased_retriggers'],[])
                    gates.append(held[0]['gate_spc_cycles'])
                    offset=sum(r['onset_intervals_spc_cycles'][2:4])
                    self.assertEqual(r['voice2_volume_updates'][0]['interval_spc_cycles'],offset)
                self.assertEqual(gates[0],gates[1])

    def test_boundary_rest_has_no_pending_KON(self):
        r=observe(io.StringIO(trace('pending-rest',127,8)),'pending-rest',127,8)
        self.assertEqual(r['keyons'][2]['mask'],8)
        self.assertEqual(r['boundary_pitch_writes'],[{'voice':3,'pitch':1200}])
        self.assertEqual(r['unreleased_retriggers'],[])

    def test_false_release_retrigger_and_pitch_metadata_reject(self):
        for mutation in ('gate','retrigger','pitch','mask','offset','type','volume','early'):
            c='rest-return' if mutation=='gate' else 'note-return'
            r=observe(io.StringIO(trace(c,127,8)),c,127,8)
            if mutation=='gate':next(g for g in r['note_gates'] if g['voice']==2 and g['onset_index']==2)['gate_spc_cycles']=30000
            elif mutation=='retrigger':r['unreleased_retriggers']=[]
            elif mutation=='pitch':r['boundary_pitch_writes'].reverse()
            elif mutation=='mask':r['keyons'][2]['mask']=8
            elif mutation=='offset':r['boundary_pitch_to_KON_spc_cycles'][0]=True
            elif mutation=='volume':r['voice2_volume_updates'][0]['volumes']=[7,7]
            elif mutation=='early':r['voice2_volume_updates'][0]['interval_spc_cycles']=0
            else:r['note_gates'][0]['voice']=True
            with self.assertRaises(ValueError):contract(r,c)

    def test_complete_intermodel_matrix_and_faults(self):
        refs=references();self.assertEqual(len(agree(refs)),16)
        for mutation in ('duplicate','gate','offset','onset','type'):
            bad=copy.deepcopy(refs)
            if mutation=='duplicate':bad[-1]=bad[0]
            elif mutation=='gate':bad[1]['note_gates'][0]['gate_spc_cycles']+=3000
            elif mutation=='offset':bad[1]['boundary_pitch_to_KON_spc_cycles'][0]=3000
            elif mutation=='onset':
                bad[1]['onset_intervals_spc_cycles'][0]+=3000
            else:bad[0]['pending_duration']=True
            with self.assertRaises(ValueError):agree(bad)
        bad=copy.deepcopy(refs)
        for r in bad:
            if r['case']=='rest-return' and r['articulation']==127 and r['pending_duration']==16:
                next(g for g in r['note_gates'] if (g['onset_index'],g['voice'])==(2,2))['gate_spc_cycles']+=3000
        with self.assertRaises(ValueError):agree(bad)
        with self.assertRaises(ValueError):agree(refs[:-1])

    def test_malformed_bounded_trace_and_input_reject(self):
        for text in ('bad\n','x'*(16*1024*1024+1),trace('note-return',127,8).replace(',76,12\n',',76,4\n',1)):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'note-return')
        for args in ((True,8,'note-return'),(127,True,'note-return'),(127,24,'note-return'),(127,8,'unknown')):
            with self.assertRaises(ValueError):bank(*args)

    def test_native_rejection_cannot_claim_support(self):
        r=native(PROBE,bank());rejection(r)
        for mutation in ('status','events','writes'):
            bad=copy.deepcopy(r)
            if mutation=='status':bad['status']=2
            elif mutation=='events':bad['events']=[{}]
            else:bad['voice_writes']=[{}]
            with self.assertRaises(ValueError):rejection(bad)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
