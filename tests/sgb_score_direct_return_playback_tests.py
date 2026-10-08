#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical direct-return retrigger, setup, final audio and lifecycle checks."""
import argparse,copy,hashlib
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_direct_return import build,source
from build_sgb_score_final_mixed import build as prior_build
from build_sgb_score_direct_return_fixture import bank,CASES,ARTICULATIONS,DURATIONS
from check_sgb_score_direct_return_playback import native,validate,identity
from check_sgb_score_final_playback import observation,control_writes
from check_sgb_score_polygate_reference import native as gate_native
from build_sgb_prototype import assemble
PROBE=None


class DirectReturnPlaybackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.reports={f'{c}-{a}-{d}':native(PROBE,bank(a,d,c)) for c in CASES for a in ARTICULATIONS for d in DURATIONS}

    def test_reproducibility_size_predecessor_and_owned_matrix(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4089)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'11eba7bf28920d59fd0278de5149263fc6803328554d4c030f8cd2e2f998440a')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'16f78157cb9c49d4d4b22f7776223c77f81b9fd9aad2a64414892cbd508d882d')
        for r in self.reports.values():
            c,a,d=identity(r);self.assertEqual(r['pattern_ticks'],[0,32,64]);self.assertEqual(r['end_tick'],64+d)
            self.assertEqual([e['articulation'] for e in r['events']],[127]*6+[a]*3)
            self.assertEqual(r['direct_return_mode'],1)

    def test_old_voice_stays_keyed_until_return_retrigger_without_KOF(self):
        for r in self.reports.values():
            old=r['envelopes'][2];new=r['envelopes'][6]
            self.assertGreater(r['frozen_peer_checks'][0],300000)
            self.assertEqual(old['off_half_cycle'],0);self.assertFalse(old['release_zero'])
            self.assertEqual(old['retrigger_half_cycle'],new['on_half_cycle'])
            self.assertTrue(new['attack_zero']);self.assertEqual(new['peak'],127)
            gates,held=observation(r,allow_retrigger=True)
            self.assertEqual([(g['onset_index'],g['voice']) for g in gates],[(0,2),(0,3),(1,3),(2,3),(3,3),(4,2)])
            self.assertTrue(all(g['cause']==1 for g in gates));self.assertTrue(new['release_zero'])
            self.assertLess(new['off_half_cycle'],control_writes(r)[0]['half_cycle'])

    def test_pitch_before_volume_and_exact_return_KOF_KON_sequence(self):
        for r in self.reports.values():
            start,stop=(r['events'][i]['half_cycle'] for i in (6,7))
            ws=[w for w in r['voice_writes'] if start<=w['half_cycle']<stop]
            self.assertEqual([(w['address'],w['value']) for w in ws],[(34,164),(35,6),(32,7),(33,7)])
            half=r['keyons'][4]['half_cycle'];keys=[w for w in r['key_writes'] if w['half_cycle']<=half][-3:]
            self.assertEqual([(w['address'],w['value']) for w in keys],[(92,0),(92,0),(76,4)])
            self.assertEqual(r['keyons'][4]['held_mask'],0)

    def test_final_FF_is_retained_without_inventing_a_peer_release(self):
        for r in self.reports.values():
            c,a,d=identity(r);pulse=control_writes(r)
            self.assertEqual([(w['address'],w['value']) for w in pulse],[(92,255),(92,0),(76,4 if c=='direct-note' else 0)])
            self.assertTrue(all(e['cause']==1 for e in r['keyoffs']))
            self.assertEqual(len(r['keyoffs']),5)
            if c=='direct-note':
                self.assertEqual(r['keyons'][-1]['pitches'][0],1800);self.assertEqual(r['keyons'][-1]['volumes'][0],[7,7])
                self.assertEqual(r['envelopes'][-1]['off_half_cycle'],0)

    def test_final_audio_reset_save_load_and_cross_engine_continuation(self):
        for r in self.reports.values():
            c,a,d=identity(r);self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal'])
            self.assertTrue(r['source_unmodified']);self.assertTrue(r['cache_guards_equal'])
            self.assertEqual(r['final_observation_half_cycles'],600000)
            if c=='direct-note':self.assertGreater(r['final_tail_pcm']['nonzero_frames'],0);self.assertGreater(r['final_env_end'],0)
            else:self.assertEqual(r['final_tail_pcm']['nonzero_frames'],0);self.assertEqual(r['final_env_end'],0)

    def test_unmeasured_simultaneous_profiles_reject_silently(self):
        mutations=[]
        for offset,value in ((0x6DF,0xA1),(0x6E4,0xA0),(0x6E2,16),(0x6DE,127),(0x6BE,127),(0x2FC,63)):
            b=bytearray(bank());b[offset]=value;mutations.append(bytes(b))
        b=bytearray(bank());b[0x6DD]=24;b[0x6BD]=24;b[0x6E2]=24;mutations.append(bytes(b))
        for b in mutations:
            r=native(PROBE,b);self.assertEqual(r['status'],226);self.assertEqual(r['key_writes'],[])

    def test_predecessor_remains_silent_for_the_new_corpus(self):
        for c in CASES:
            r=gate_native(PROBE,bank(case=c),builder=prior_build,validator=validate,output_bound=131072)
            self.assertEqual(r['status'],226)

    def test_return_release_order_profile_and_freeze_faults_fail(self):
        for before,after in (('    call direct_return_voice\n','    call pending_mix\n    call duet_voice\n'),
                             ('    mov $71, #$00\n    mov $f2, #$5c\n    mov $f3, #$00\ndirect_return_on_old:',
                              '    mov $71, #$04\n    mov $f2, #$5c\n    mov $f3, #$04\ndirect_return_on_old:'),
                             ('    mov $71, #$00\n    mov $f2, #$5c\n    mov $f3, #$00\ndirect_return_on_old:',
                              '    mov $78, #$0f\n    mov $71, #$00\n    mov $f2, #$5c\n    mov $f3, #$00\ndirect_return_on_old:'),
                             ('reselect_pulse2:\n    mov a, $a3\n    and a, #$04\n    beq gates_pulse3\n','reselect_pulse2:\n')):
            broken=source().replace(before,after,1);self.assertNotEqual(broken,source())
            with self.assertRaises(ValueError):gate_native(PROBE,bank(),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_retrigger_controls_timer_and_tail_metadata_forgeries_fail(self):
        for mutation in ('retrigger','attack','release','return-control','final-control','mode','tail','restore'):
            r=copy.deepcopy(self.reports['direct-note-63-8'])
            if mutation=='retrigger':r['envelopes'][2]['retrigger_half_cycle']=0
            elif mutation=='attack':r['envelopes'][6]['attack_zero']=False
            elif mutation=='release':r['keyoffs'][-1]['cause']=0
            elif mutation=='return-control':next(w for w in r['key_writes'] if w['half_cycle']==r['keyons'][4]['half_cycle'])['value']=0
            elif mutation=='final-control':control_writes(r)[0]['value']=4
            elif mutation=='mode':r['direct_return_mode']=0
            elif mutation=='tail':r['final_tail_pcm']['nonzero_frames']=0
            elif mutation=='restore':r['restore_equal']=False
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(r)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
