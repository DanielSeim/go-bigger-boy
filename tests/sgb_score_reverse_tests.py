#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Executed but unkeyed boundary events, carry, physical writes and lifecycle."""
import argparse,copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_reverse import build,source
from build_sgb_score_order import build as prior_build
from build_sgb_score_reverse_fixture import bank,expected,ticks,CASES
from build_sgb_score_peer_fixture import bank as peer_bank,CASES as PEER_CASES
from check_sgb_score_reverse_reference import native,align,validate,projection,boundary_pitches,boundary_offsets,observe,contract,compare
from check_sgb_score_peer_reference import native_observation
from check_sgb_score_polygate_reference import native as gate_native
from build_sgb_prototype import assemble
from check_sgb_score_gate_reference import PULSES
from schedule_sgb_score import schedule
from sgb_score_sparse_tests import authored
PROBE=None


def trace(case):
    rows=['kind,master_clock,spc_cycle,pcm_sample,address,value\n','D,0,0,0,61,0\n']
    for i,(edge,tick) in enumerate(zip(expected(case),ticks(case))):
        cycle=1000+tick*5500
        if i==2 and case in ('end-3-note','inherit-note'):
            rows.extend((f'D,0,{cycle},0,34,164\n',f'D,0,{cycle},0,35,6\n'))
        for v in edge['voices']:
            values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
            rows.extend(f'D,0,{cycle},0,{16*v["voice"]+r},{value}\n' for r,value in enumerate(values))
        rows.extend((f'D,0,{cycle},0,76,{edge["mask"]}\n',f'D,0,{cycle+20000},0,92,{edge["mask"]}\n'))
    return ''.join(rows)


class ReverseTests(unittest.TestCase):
    def test_reproducible_and_prior_unchanged(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4050)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'deece9488c7138fb4e9f0cde22beccf2edd3770f42b1df2a257f9b387cd15dda')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'2efc1a89303d11f373991727492284819dd0d2fdff8ab750d8b770c12773a56a')

    def test_all_owned_cases_and_real_edges(self):
        for c in CASES:
            for a in (63,127):
                d=bank(a,c);r=native(PROBE,d);align(r,d)
                self.assertEqual(r['status'],2);self.assertEqual(len(r['keyons']),8)
                self.assertEqual(len(r['events']),12 if c.startswith('end-2') else 13)
                self.assertEqual(projection(r)[1],[] if c.startswith('end-2') else [4])
                self.assertEqual(r['event_patterns'][:5],[0]*5 if not c.startswith('end-2') else [0,0,0,0,1])
                gates,rs=native_observation(r);self.assertEqual(len(gates),12);self.assertEqual(rs,[])
                self.assertTrue(all(g['cause']==1 for g in gates))
                self.assertEqual(len(r['envelopes']),12)

    def test_unkeyed_note_has_actual_pitch_and_mix_writes(self):
        r=native(PROBE,bank());self.assertEqual(boundary_pitches(r),[{'voice':2,'pitch':1700},{'voice':2,'pitch':1200}])
        e,n=r['events'][4:6]
        self.assertEqual((e['opcode'],e['tick'],e['duration'],e['articulation']),(0xA0,32,8,63))
        self.assertEqual(e['volumes'],[1,1]);self.assertEqual(n['opcode'],0x9A)
        self.assertFalse(any(e['half_cycle']<=k['half_cycle']<n['half_cycle'] for k in r['keyons']+r['keyoffs']))
        self.assertEqual(r['keyons'][2]['pitches'][0],1200)
        self.assertEqual(r['keyons'][2]['pending_pulses'][0],36)

    def test_executed_note_and_rest_seed_implicit_timing(self):
        for c in ('inherit-note','inherit-rest'):
            for a in (63,127):
                d=bank(a,c);r=native(PROBE,d);align(r,d)
                self.assertEqual(r['pattern_ticks'],[0,32,48,80]);self.assertEqual(r['end_tick'],112)
                self.assertEqual([(e['duration'],e['articulation'],e['track_volume']) for e,p in zip(r['events'],r['event_patterns']) if p==1],[(8,63,64)]*2)
                self.assertEqual(r['keyons'][2]['pending_pulses'][0],10)
                self.assertEqual(len([w for w in r['voice_writes'] if r['events'][4]['half_cycle']<=w['half_cycle']<r['events'][5]['half_cycle']]),4 if c.endswith('note') else 0)

    def test_conservative_guard_and_previous_image_reject(self):
        peer=(16,127,0x98,0x99,8,63,0xA0,0);end=(16,127,0x98,0x99,0)
        for later in (None,((None,(16,127,0x9C,0)),),(((16,127,0xC9,0x9C,0),None),)):
            data=authored([(peer,end),*(later or ())]);r=native(PROBE,data)
            self.assertEqual(r['status'],226);self.assertEqual(r['events'],[]);self.assertEqual(r['voice_writes'],[])
            self.assertEqual(r['pcm']['nonzero_frames'],0);self.assertEqual(r['keyons'],[])
        r=gate_native(PROBE,bank(),builder=prior_build,validator=validate,output_bound=131072)
        self.assertEqual(r['status'],226);self.assertEqual(r['events'],[])

    def test_missing_timing_order_fault_is_detected(self):
        s=source().replace('    beq timing_done\n    call timing_load2\ntiming_check3:', '    beq timing_done\ntiming_check3:')
        s=s.replace('timing_next2:\n', '''timing_next2:
    mov a, $a2
    and a, #$04
    beq timing_next3
    mov a, $32
    bne timing_next3
    call timing_load2
''')
        image=bytes(assemble(s,'spc',0x0800))
        # The runtime logs still execute the event, but parser carry is wrong.
        r=gate_native(PROBE,bank(127,'inherit-note'),builder=lambda:image,validator=validate,output_bound=131072)
        with self.assertRaises(ValueError):align(r,bank(127,'inherit-note'))

    def test_all_profiles_and_full_log(self):
        for tempo,art,duration in PULSES:
            note=(0xE0,2,duration,art,*([0x98]*8),0)
            later=(*([0x99]*8),0)
            data=authored([(note,note),*([(later,later)]*3)])
            r=native(PROBE,data,tempo);align(r,data)
            self.assertEqual(len(r['events']),64);self.assertEqual(projection(r)[1],[])
            self.assertEqual(len(native_observation(r)[0]),64)

    def test_preceding_clipped_peer_lifecycles(self):
        for c in PEER_CASES:
            d=peer_bank(127,c);r=native(PROBE,d);align(r,d)

    def test_oracle_opt_in_keeps_default_rejection(self):
        d=bank();root=int.from_bytes(d[:2],'little')
        for kwargs in ({},{'end_priority':True}):
            with self.assertRaises(ValueError):schedule(d,root,inherit_timing=True,**kwargs)
        s=schedule(d,root,inherit_timing=True,end_priority=True,boundary_events=True)
        timed=[e for e in s['events'] if e['kind'] in ('note','rest')]
        self.assertEqual(len(timed),13);self.assertTrue(timed[4]['boundary_event']);self.assertEqual(timed[4]['end_tick'],32)
        for kwargs in ({'boundary_events':True},{'end_priority':True,'boundary_events':1}):
            with self.assertRaises(ValueError):schedule(d,root,**kwargs)

    def test_false_raw_event_pattern_write_and_key_metadata(self):
        r=native(PROBE,bank())
        for mutation in ('pattern','opcode','art','volume','type','write','missing','key'):
            bad=copy.deepcopy(r)
            if mutation=='pattern':bad['event_patterns'][4]=1
            elif mutation=='opcode':bad['events'][4]['opcode']=0x9A
            elif mutation=='art':bad['events'][4]['articulation']=64
            elif mutation=='volume':bad['events'][4]['track_volume']=127
            elif mutation=='type':bad['events'][4]['volumes']=[True,True]
            elif mutation=='write':next(w for w in bad['voice_writes'] if w['half_cycle']>bad['events'][4]['half_cycle'] and w['address']==0x22)['value']=0
            elif mutation=='missing':bad['voice_writes'].pop()
            else:bad['keyons'][2]['half_cycle']=bad['events'][4]['half_cycle']+1
            with self.assertRaises(ValueError):validate(bad)

    def test_reference_matrix_and_mutations(self):
        candidates={f'{c}-{a}':native(PROBE,bank(a,c)) for c in CASES for a in (63,127)};refs=[]
        for c in CASES:
            for a in (63,127):
                result=observe(io.StringIO(trace(c)),c);r=candidates[f'{c}-{a}'];gates,rs=native_observation(r)
                result['note_gates']=[{'voice':g['voice'],'onset_index':g['onset_index'],'gate_spc_cycles':int(g['gate_spc_cycles']),'release_kind':'keyoff'} for g in gates]
                result['boundary_pitch_to_KON_spc_cycles']=[int(v) for v in boundary_offsets(r)]
                result['onset_intervals_spc_cycles']=[int((b['half_cycle']-a['half_cycle'])/2) for a,b in zip(r['keyons'],r['keyons'][1:])]
                for m in ('sgb','sgb2'):refs.append({**copy.deepcopy(result),'case':c,'articulation':a,'model':m})
        self.assertEqual(len(compare(candidates,refs)),24)
        for mutation in ('gate','duplicate','pitch','type','offset'):
            bad=copy.deepcopy(refs)
            if mutation=='gate':bad[0]['note_gates'][0]['gate_spc_cycles']+=6000
            elif mutation=='duplicate':bad[-1]=bad[0]
            elif mutation=='pitch':bad[8]['boundary_pitch_writes'].reverse()
            elif mutation=='offset':bad[0]['boundary_pitch_to_KON_spc_cycles'][0]=True
            else:bad[0]['note_gates'][0]['voice']=True
            with self.assertRaises(ValueError):compare(candidates,bad)
        with self.assertRaises(ValueError):observe(io.StringIO(trace('end-3-note').replace(',35,6\n',',35,5\n',1)),'end-3-note')


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);a,rest=p.parse_known_args();PROBE=a.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
