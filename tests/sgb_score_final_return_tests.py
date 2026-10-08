#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Final note/rest after clipped-voice inactivity and measured returning rest."""
import argparse,copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_final_return import build,source
from build_sgb_score_final_peer import build as prior_build
from build_sgb_score_final_return_fixture import bank,build as fixture_build,CASES,RESTS,DURATIONS
from check_sgb_score_final_return_reference import expected,observe,contract,agree
from check_sgb_score_final_return_playback import native,validate,identity,observation,control_writes
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_multi_reference import validate as multi_validate
from check_sgb_score_final_playback import SCHEMA as PRIOR_SCHEMA,validate as prior_validate
from build_sgb_score_final_fixture import bank as prior_bank
from check_sgb_score_gate_reference import PULSES
from build_sgb_prototype import assemble
PROBE=None


def trace(c,a,d,s):
    rows=[(0,61,0)]
    for i,e in enumerate(expected(c)):
        cycle=10000+(i*16 if i<4 else 64+s)*5500
        for v in e['voices']:
            values=(*v['volumes'],v['pitch']&255,v['pitch']>>8,2,143,111,184)
            rows.extend((cycle,16*v['voice']+r,x) for r,x in enumerate(values))
            if i<4:
                if (i,v['voice'],a)==(1,2,127):off=10000+64*5500+(5 if s==4 else 15)*2048
                else:off=cycle+PULSES[(96,a,24 if (i,v['voice'])==(1,2) else 16)]*2048
                rows.append((off,92,1<<v['voice']))
        # The terminal note preserves actual volume and writes only pitch.
        if i==4:rows=[r for r in rows if not(r[0]==cycle and r[1] in (32,33))]
        rows.append((cycle,76,e['mask']))
    stop=10000+(64+s)*5500-156
    if c=='return-note':
        rows=[r for r in rows if not(r[0]>10000+48*5500 and r[1] in (34,35))]
        rows.extend(((stop-500,34,164),(stop-464,35,6)))
    rows.extend(((stop,92,255),(stop+117,92,0),(stop+156,76,4 if c=='return-note' else 0),(stop+1000,92,0),(stop+400000,61,0)))
    # Only one terminal KON write is present.
    rows=list(dict.fromkeys(rows));rows.sort(key=lambda r:r[0])
    return 'kind,master_clock,spc_cycle,pcm_sample,address,value\n'+''.join(f'D,0,{t},0,{r},{v}\n' for t,r,v in rows)


def references():
    result=[]
    for c in CASES:
        for s in RESTS:
            for d in DURATIONS:
                for a in (63,127):
                    r=observe(io.StringIO(trace(c,a,d,s)),c,a,d,s)
                    result.extend({**copy.deepcopy(r),'case':c,'model':m} for m in ('sgb','sgb2'))
    return result


class FinalReturnTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={f'{c}-{s}-{d}-{a}':native(PROBE,bank(a,d,c,s)) for c in CASES for s in RESTS for d in DURATIONS for a in (63,127)}

    def test_owned_reproducible_matrix_and_preceding_hash(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4089)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'e220d4f928b3e4fb572d67d630a5691b4aeedd57abf04d5c636bbc3124ee06d9')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'09c6b3f1b641cd232f936a68b507be239a789766e7b7ec60f606556710db36e5')
        hashes={hashlib.sha256(fixture_build(a,d,c,s)).hexdigest() for c in CASES for s in RESTS for d in DURATIONS for a in (63,127)}
        self.assertEqual(len(hashes),16)
        for r in self.reports.values():
            c,a,d,s=identity(r);self.assertEqual(len(bank(a,d,c,s)),2048)
            self.assertEqual(r['pattern_ticks'],[0,32,64]);self.assertEqual(r['end_tick'],64+s)
            self.assertEqual(r['events'][-1]['tick'],r['end_tick'])
            with self.assertRaises(ValueError):multi_validate(r,schema=r['schema'],max_events=64,max_ticks=2032,min_events=1,initial_pair=False)

    def test_returning_rest_rearms_the_existing_voice_without_KON(self):
        for r in self.reports.values():
            c,a,d,s=identity(r);gates,held=observation(r)
            self.assertEqual(len(gates),6);self.assertTrue(all(g['cause']==1 for g in gates))
            self.assertEqual(held,[2] if c=='return-note' else [])
            self.assertNotIn(64,[e['tick'] for e in r['keyons']])
            old=next(g for g in gates if (g['onset_index'],g['voice'])==(1,2))
            if a==127:
                self.assertGreater(r['frozen_peer_checks'][0],300000)
                self.assertGreater(old['gate_spc_cycles'],240000)
                off=next(e for e in r['keyoffs'] if e['mask']&4 and e['half_cycle']>r['keyons'][1]['half_cycle'])
                self.assertLess(off['half_cycle'],r['events'][-1]['half_cycle'])
                pulses=5 if s==4 else 15
                self.assertLessEqual(abs((off['half_cycle']-r['events'][6]['half_cycle'])/2-pulses*2048),4096)
            else:self.assertEqual(r['frozen_peer_checks'][0],0)

    def test_final_note_is_a_new_attack_after_real_release(self):
        for key,r in self.reports.items():
            c,a,d,s=identity(r)
            if c=='return-note':
                self.assertEqual([e['mask'] for e in r['keyons']],[12,12,8,8,4])
                self.assertTrue(all(e['retrigger_half_cycle']==0 for e in r['envelopes']))
                old=[e for e in r['envelopes'] if e['voice']==2][1];new=r['envelopes'][-1]
                self.assertGreater(old['off_half_cycle'],old['on_half_cycle'])
                self.assertLess(old['off_half_cycle'],new['on_half_cycle'])
                self.assertTrue(old['release_zero']);self.assertTrue(new['attack_zero']);self.assertEqual(new['peak'],127)
                self.assertEqual(new['off_half_cycle'],0);self.assertGreater(r['final_env_end'],0)

    def test_final_actual_pitch_volume_and_stop_pulse(self):
        for r in self.reports.values():
            c,a,d,s=identity(r);last=r['events'][-1]
            self.assertEqual(last['track_volume'],64)
            actual=[w for w in r['voice_writes'] if w['half_cycle']>last['half_cycle']]
            self.assertEqual([w['address'] for w in actual],[34,35] if c=='return-note' else [])
            self.assertEqual([(w['address'],w['value']) for w in control_writes(r)],[(92,255),(92,0),(76,4 if c=='return-note' else 0)])
            if c=='return-note':self.assertEqual(r['keyons'][-1]['volumes'][0],[7,7])

    def test_final_held_or_silent_PCM_restores_and_resets(self):
        for r in self.reports.values():
            c,a,d,s=identity(r);self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal'])
            self.assertEqual(r['final_observation_half_cycles'],600000)
            self.assertGreater(r['final_tail_pcm']['nonzero_frames'],0) if c=='return-note' else self.assertEqual(r['final_tail_pcm']['nonzero_frames'],0)
            if c=='return-rest':self.assertEqual(r['final_env_end'],0);self.assertGreaterEqual(r['pcm']['quiet_tail_frames'],64)

    def test_unknown_duration_articulation_geometry_and_note4_remain_silent(self):
        mutations=[];data=bank()
        changed=bytearray(data);changed[0x6DD]=6;changed[0x6BD]=6;mutations.append(bytes(changed))
        for offset,value in ((0x6DD+1,63),(0x6DD+5,24),(0x6DD+2,0x98)):
            changed=bytearray(data);changed[offset]=value;mutations.append(bytes(changed))
        changed=bytearray(data);changed[0x2F1+13]=16;mutations.append(bytes(changed))
        for data in mutations:
            r=native(PROBE,data);self.assertEqual(r['status'],226);self.assertEqual(r['key_writes'],[])
        # Non-simultaneous ends keep preceding completion; they cannot be
        # qualified by the new returning-rest/final-ready checker.
        changed=bytearray(bank());changed[0x6BD]=8
        r=gate_native(PROBE,bytes(changed),builder=build,validator=lambda r:None,output_bound=131072)
        self.assertEqual(r['status'],2);self.assertEqual(r['final_return_mode'],0);self.assertEqual(r['final_mode'],0)
        with self.assertRaises(ValueError):validate(r)
        with self.assertRaises(ValueError):bank(rest=True)
        with self.assertRaises(ValueError):bank(duration=4)

    def test_preceding_final_image_stays_independently_qualified(self):
        for c in CASES:
            r=gate_native(PROBE,bank(case=c),builder=prior_build,validator=validate,output_bound=131072)
            self.assertEqual(r['status'],226)
        for c in ('final-note','final-rest'):
            r=gate_native(PROBE,prior_bank(case=c),builder=build,validator=lambda r:prior_validate({**r,'schema':PRIOR_SCHEMA}),output_bound=131072)
            self.assertEqual(r['final_return_mode'],0)

    def test_removed_rest_rearm_freeze_or_final_KON_fails(self):
        for before,after in (('    call final_return_rest_gate\n',''),
                             ('reselect_pulse2:\n    mov a, $a3\n    and a, #$04\n    beq gates_pulse3\n','reselect_pulse2:\n'),
                             ('    mov a, $51\n    mov $f3, a\n    beq final_clear\n','    mov a, #$00\n    mov $f3, a\n    beq final_clear\n')):
            broken=source().replace(before,after,1);self.assertNotEqual(broken,source())
            with self.assertRaises(ValueError):gate_native(PROBE,bank(),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_false_native_release_and_terminal_metadata(self):
        original=self.reports['return-note-4-8-127']
        for mutation in ('cause','pitch','volume','held','env','gate','mode','key-write','PCM','restore'):
            r=copy.deepcopy(original)
            if mutation=='cause':r['keyoffs'][-1]['cause']=0
            elif mutation=='pitch':r['keyons'][-1]['pitches'][0]=1132
            elif mutation=='volume':r['keyons'][-1]['volumes'][0]=[1,1]
            elif mutation=='held':r['envelopes'][-1]['off_half_cycle']=r['completion_half_cycle']
            elif mutation=='env':r['final_env_end']=0
            elif mutation=='gate':r['keyoffs'][-1]['half_cycle']+=20000
            elif mutation=='mode':r['final_return_mode']=0
            elif mutation=='key-write':r['key_writes'][-2]['value']=0
            elif mutation=='PCM':r['final_tail_pcm']['nonzero_frames']=0
            elif mutation=='restore':r['restore_equal']=False
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(r)

    def test_synthetic_opaque_matrix_and_reference_guards(self):
        refs=references();self.assertEqual(len(agree(refs)),16)
        for mutation in ('held','retrigger','source','return-gate','pulse','pitch','volume','window','type'):
            r=copy.deepcopy(next(r for r in refs if r['case']=='return-note' and r['articulation']==127))
            if mutation=='held':r['voices_pending_at_stop']=[2]
            elif mutation=='retrigger':r['unreleased_retriggers']=[2]
            elif mutation=='source':r['note_gates'][2]['release_kind']='final-stop'
            elif mutation=='return-gate':r['return_release_after_previous_onset_spc_cycles']=1
            elif mutation=='pulse':r['final_control_writes'][0]['value']=12
            elif mutation=='pitch':r['boundary_pitch_writes'][0]['pitch']=1132
            elif mutation=='volume':r['post_previous_onset_volume_writes']=[{'address':32,'value':1}]
            elif mutation=='window':r['post_stop_observation_spc_cycles']=1
            elif mutation=='type':r['return_rest']=True
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):contract(r,r['case'])
        with self.assertRaises(ValueError):agree(refs[:-1])
        with self.assertRaises(ValueError):agree(refs[:-1]+[refs[0]])
        drift=copy.deepcopy(refs);drift[1]['onset_intervals_spc_cycles'][0]+=3000
        with self.assertRaises(ValueError):agree(drift)
        for text in ('bad header\n','x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'return-note')
        rows='kind,master_clock,spc_cycle,pcm_sample,address,value\n'+'D,0,0,0,61,0\n'*32768
        with self.assertRaisesRegex(ValueError,'row bound'):observe(io.StringIO(rows),'return-note')


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
