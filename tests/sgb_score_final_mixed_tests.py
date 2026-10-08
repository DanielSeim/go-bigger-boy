#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Held articulation-127 voice returning with articulation-63 rests."""
import argparse,copy,hashlib,io
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_final_mixed import build,source
from build_sgb_score_final_return import build as prior_build
from build_sgb_score_final_mixed_fixture import bank,build as fixture_build,CASES,RESTS,DURATIONS
from build_sgb_score_final_return_fixture import bank as uniform_bank
from check_sgb_score_final_mixed_playback import native,validate,identity,observation,control_writes,SCHEMA
from check_sgb_score_final_mixed_reference import contract,agree
from check_sgb_score_final_return_reference import observe
from check_sgb_score_final_return_playback import validate as uniform_validate,SCHEMA as UNIFORM_SCHEMA
from check_sgb_score_polygate_reference import native as gate_native
from build_sgb_prototype import assemble
from sgb_score_final_return_tests import trace as prior_trace
PROBE=None


def references():
    refs=[]
    for c in CASES:
        for s in RESTS:
            for d in DURATIONS:
                text=prior_trace(c,127,d,s)
                if s==8:
                    old=10000+64*5500+15*2048;new=10000+64*5500+9*2048
                    rows=text.splitlines();header=rows.pop(0)
                    rows=[row.replace(f'D,0,{old},0,92,4',f'D,0,{new},0,92,4') for row in rows]
                    rows.sort(key=lambda row:int(row.split(',')[2]));text=header+'\n'+'\n'.join(rows)+'\n'
                r=observe(io.StringIO(text),c,127,d,s,return_art=63)
                refs.extend({**copy.deepcopy(r),'case':c,'model':m} for m in ('sgb','sgb2'))
    return refs


class FinalMixedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={f'{c}-{s}-{d}':native(PROBE,bank(127,d,c,s)) for c in CASES for s in RESTS for d in DURATIONS}

    def test_owned_matrix_and_preceding_hash(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4089)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'16f78157cb9c49d4d4b22f7776223c77f81b9fd9aad2a64414892cbd508d882d')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'e220d4f928b3e4fb572d67d630a5691b4aeedd57abf04d5c636bbc3124ee06d9')
        self.assertEqual(len({hashlib.sha256(fixture_build(127,d,c,s)).hexdigest() for c in CASES for s in RESTS for d in DURATIONS}),8)
        for r in self.reports.values():
            self.assertEqual([e['articulation'] for e in r['events']],[127]*6+[63]*3)
            self.assertEqual(r['pattern_ticks'],[0,32,64]);self.assertEqual(r['pattern_masks'],[12,8,12])

    def test_measured_rest_releases_frozen_live_voice_without_KON(self):
        for r in self.reports.values():
            c,a,d,s=identity(r);gates,held=observation(r)
            self.assertEqual(len(gates),6);self.assertTrue(all(g['cause']==1 for g in gates))
            self.assertEqual(held,[2] if c=='return-note' else [])
            self.assertGreater(r['frozen_peer_checks'][0],300000)
            self.assertNotIn(64,[e['tick'] for e in r['keyons']])
            off=next(e for e in r['keyoffs'] if e['mask']&4 and e['half_cycle']>r['keyons'][1]['half_cycle'])
            self.assertLess(off['half_cycle'],r['events'][-1]['half_cycle'])
            self.assertLessEqual(abs((off['half_cycle']-r['events'][6]['half_cycle'])/2-(5 if s==4 else 9)*2048),4096)

    def test_new_final_attack_or_silent_rest_with_deferred_volume(self):
        for r in self.reports.values():
            c,a,d,s=identity(r);last=r['events'][-1]
            self.assertEqual(last['track_volume'],64)
            self.assertEqual([(w['address'],w['value']) for w in control_writes(r)],[(92,255),(92,0),(76,4 if c=='return-note' else 0)])
            if c=='return-note':
                old=[e for e in r['envelopes'] if e['voice']==2][1];new=r['envelopes'][-1]
                self.assertTrue(old['release_zero']);self.assertTrue(new['attack_zero'])
                self.assertLess(old['off_half_cycle'],new['on_half_cycle'])
                self.assertEqual(new['retrigger_half_cycle'],0);self.assertEqual(new['off_half_cycle'],0)
                self.assertEqual(r['keyons'][-1]['volumes'][0],[7,7]);self.assertEqual(r['keyons'][-1]['pitches'][0],1700)
                self.assertGreater(r['final_env_end'],0);self.assertGreater(r['final_tail_pcm']['nonzero_frames'],0)
            else:
                self.assertEqual(r['final_env_end'],0);self.assertEqual(r['final_tail_pcm']['nonzero_frames'],0)
                self.assertGreaterEqual(r['pcm']['quiet_tail_frames'],64)

    def test_reset_save_and_cross_engine_continuation(self):
        for r in self.reports.values():
            self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal'])
            self.assertEqual(r['final_observation_half_cycles'],600000)
            self.assertTrue(r['source_unmodified']);self.assertTrue(r['cache_guards_equal'])

    def test_unknown_transitions_and_geometry_reject_silently(self):
        mutations=[]
        for offset,value in ((0x6DE,127),(0x6E3,127),(0x6BE,127),(0x2F2,63),(0x6E2,24)):
            data=bytearray(bank());data[offset]=value;mutations.append(bytes(data))
        data=bytearray(bank());data[0x6DD]=6;data[0x6BD]=6;mutations.append(bytes(data))
        data=bytearray(uniform_bank(63));data[0x6DE]=127;data[0x6E3]=127;data[0x6BE]=127;mutations.append(bytes(data))
        for data in mutations:
            r=native(PROBE,data);self.assertEqual(r['status'],226);self.assertEqual(r['key_writes'],[])
        with self.assertRaises(ValueError):bank(63)
        with self.assertRaises(ValueError):bank(True)

    def test_uniform_profiles_still_validate_and_predecessor_rejects_mixed(self):
        for a in (63,127):
            for s in RESTS:
                r=gate_native(PROBE,uniform_bank(a,8,'return-note',s),builder=build,validator=lambda r:uniform_validate({**r,'schema':UNIFORM_SCHEMA}),output_bound=131072)
                self.assertEqual(r['status'],2)
        r=gate_native(PROBE,bank(),builder=prior_build,validator=validate,output_bound=131072)
        self.assertEqual(r['status'],226)

    def test_rest_profile_and_freeze_faults_fail_physical_checks(self):
        for before,after,data in (('    mov $72, #$09\n','    mov $72, #$0f\n',bank(rest=8)),
                                  ('    call final_return_rest_gate\n','',bank()),
                                  ('reselect_pulse2:\n    mov a, $a3\n    and a, #$04\n    beq gates_pulse3\n','reselect_pulse2:\n',bank())):
            broken=source().replace(before,after,1);self.assertNotEqual(broken,source())
            with self.assertRaises(ValueError):gate_native(PROBE,data,builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_native_articulation_release_and_tail_forgeries_fail(self):
        for mutation in ('initial','return','final','release','mode','PCM','restore'):
            r=copy.deepcopy(self.reports['return-note-8-8'])
            if mutation=='initial':r['events'][0]['articulation']=63
            elif mutation=='return':r['events'][6]['articulation']=127
            elif mutation=='final':r['events'][-1]['articulation']=127
            elif mutation=='release':r['keyoffs'][-1]['cause']=0
            elif mutation=='mode':r['final_return_mode']=0
            elif mutation=='PCM':r['final_tail_pcm']['nonzero_frames']=0
            elif mutation=='restore':r['restore_equal']=False
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(r)

    def test_bounded_synthetic_reference_matrix_and_raw_gate_binding(self):
        refs=references();self.assertEqual(len(agree(refs)),8)
        for field,value in (('return_articulation',127),('articulation',63),('return_rest',True),('return_release_after_previous_onset_spc_cycles',1),('voices_pending_at_stop',[2]),('post_stop_observation_spc_cycles',1)):
            r=copy.deepcopy(refs[0]);r[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):contract(r,r['case'])
        with self.assertRaises(ValueError):agree(refs[:-1])
        with self.assertRaises(ValueError):agree(refs[:-1]+[refs[0]])
        for text in ('bad header\n','x'*(16*1024*1024+1)):
            with self.assertRaises(ValueError):observe(io.StringIO(text),'return-note',return_art=63)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
