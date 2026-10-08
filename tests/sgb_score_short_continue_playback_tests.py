#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Guarded continuation, deferred mix, physical DSP and complete lifecycle."""
import argparse,copy,hashlib
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_short_continue import build,source
from build_sgb_score_short_return import build as prior_build
from build_sgb_score_short_continue_fixture import bank,CASES,ARTICULATIONS,DURATIONS
from build_sgb_score_short_return_fixture import bank as prior_bank
from build_sgb_score_short_pair_fixture import bank as pair_bank
from check_sgb_score_short_continue_playback import native,validate,identity
from check_sgb_score_short_return_playback import validate as prior_validate,SCHEMA as PRIOR_SCHEMA
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_final_playback import observation,control_writes
from check_sgb_score_gate_reference import PULSES
from build_sgb_prototype import assemble
PROBE=None


class ShortContinuePlaybackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.reports={f'{c}-{a}-{d}':native(PROBE,bank(a,d,c)) for c in CASES for a in ARTICULATIONS for d in DURATIONS}

    def test_reproducibility_descriptor_relocation_and_unchanged_gate_table(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4081)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'c0a890f508c30169591ccfcb865113b4198c31af2ef6960fa03256180cbaef70')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'d86d1587cb4329b9ded58e42a14d589b66af92fca370badcf3e78c2966139ba0')
        self.assertEqual(bytes(assemble('mov a, descriptor+x\nret\ndescriptor:\n.byte $02, $8f, $6f, $b8','spc',0x0800)),bytes((0xF5,4,8,0x6F,2,143,111,184)))
        self.assertNotIn((96,63,4),PULSES);self.assertNotIn((96,127,4),PULSES)

    def test_retrigger_attack_and_release_before_ordinary_progression(self):
        for r in self.reports.values():
            old,new=r['envelopes'][2],r['envelopes'][6]
            self.assertEqual(old['off_half_cycle'],0);self.assertEqual(old['retrigger_half_cycle'],new['on_half_cycle'])
            self.assertGreater(r['frozen_peer_checks'][0],300000);self.assertTrue(new['attack_zero']);self.assertEqual(new['peak'],127);self.assertTrue(new['release_zero'])
            self.assertEqual(r['keyons'][4]['pending_pulses'][0],5)
            self.assertLess(new['off_half_cycle'],control_writes(r)[0]['half_cycle'])
            if identity(r)[0]=='continue-note':self.assertLess(new['off_half_cycle'],r['keyons'][5]['half_cycle'])

    def test_deferred_note_volume_and_existing_following_gate(self):
        for r in self.reports.values():
            c,a,d=identity(r)
            if c=='continue-note':
                e=r['keyons'][5];self.assertEqual(e['tick'],68);self.assertEqual(e['volumes'][0],[1,1]);self.assertEqual(e['pitches'][0],1800)
                self.assertEqual(e['pending_pulses'][0],PULSES[96,a,d]);self.assertEqual(e['held_mask'],0)
                env=r['envelopes'][-1];self.assertTrue(env['attack_zero']);self.assertEqual(env['peak'],127);self.assertTrue(env['release_zero'])
                self.assertLess(env['off_half_cycle'],control_writes(r)[0]['half_cycle'])
            else:self.assertEqual(len(r['keyons']),5)
            gates,held=observation(r,allow_retrigger=True);self.assertEqual(held,[])
            self.assertEqual(len(gates),7 if c=='continue-note' else 6);self.assertTrue(all(g['cause']==1 for g in gates))

    def test_pitch_before_volume_and_zero_zero_KON_sequences(self):
        for r in self.reports.values():
            for index in (6,8):
                e=r['events'][index];end=r['events'][index+1]['half_cycle'];ws=[w for w in r['voice_writes'] if e['half_cycle']<=w['half_cycle']<end]
                expected=[] if e['opcode']==0xC9 else [(34,164 if index==6 else 8),(35,6 if index==6 else 7),(32,7 if index==6 else 1),(33,7 if index==6 else 1)]
                self.assertEqual([(w['address'],w['value']) for w in ws],expected)
            for e in r['keyons'][4:]:
                ws=[w for w in r['key_writes'] if w['half_cycle']<=e['half_cycle']][-3:]
                self.assertEqual([(w['address'],w['value']) for w in ws],[(92,0),(92,0),(76,4)])

    def test_final_silence_full_tail_reset_save_load_source_and_cache(self):
        for r in self.reports.values():
            self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal']);self.assertTrue(r['source_unmodified']);self.assertTrue(r['cache_guards_equal'])
            self.assertEqual(r['end_tick'],68+identity(r)[2]);self.assertEqual(r['final_mode'],0);self.assertEqual(r['short_continue_mode'],1)
            self.assertEqual(r['final_observation_half_cycles'],600000);self.assertEqual(r['final_tail_pcm']['nonzero_frames'],0);self.assertEqual(r['final_env_end'],0)
            self.assertEqual([(w['address'],w['value']) for w in control_writes(r)],[(92,255),(92,0),(76,0)])

    def test_malformed_continuation_shapes_fail_silent_rehearsal(self):
        for offset,value in ((0x6BD,8),(0x6DF,0xA1),(0x6E2,4),(0x6C2,16),(0x6C3,127),(0x6C4,0xA0),(0x2FC,63)):
            b=bytearray(bank());b[offset]=value;r=native(PROBE,bytes(b))
            self.assertEqual(r['status'],226);self.assertEqual(r['key_writes'],[]);self.assertEqual(r['pcm']['nonzero_frames'],0)
        b=bytearray(bank());b[0x6C5:0x6C9]=bytes((8,63,0xC9,0));r=native(PROBE,bytes(b));self.assertEqual(r['status'],226)

    def test_predecessor_rejects_continuing_banks_and_old_short_profile_survives(self):
        for c in CASES:
            r=gate_native(PROBE,bank(case=c),builder=prior_build,validator=validate,output_bound=131072);self.assertEqual(r['status'],226)
        for c in ('direct-note','direct-rest'):
            r=gate_native(PROBE,prior_bank(case=c),builder=build,validator=lambda r:prior_validate({**r,'schema':PRIOR_SCHEMA}),output_bound=131072)
            self.assertEqual(r['short_continue_mode'],0)

    def test_two_short_event_profiles_remain_silently_rejected(self):
        for c in CASES:
            for a in ARTICULATIONS:
                r=native(PROBE,pair_bank(a,c))
                self.assertEqual(r['status'],226)
                self.assertEqual(r['key_writes'],[])
                self.assertEqual(r['pcm']['nonzero_frames'],0)

    def test_counter_order_completion_and_profile_flag_faults_fail(self):
        for before,after in (('    mov $76, #$05\n','    mov $76, #$0f\n'),('short_continue_voice:\n    call duet_voice\n    jmp pending_mix\n','short_continue_voice:\n    call pending_mix\n    jmp duet_voice\n'),('    mov $c5, #$01\n','    mov $c5, #$00\n'),('    mov $51, #$00\n    jmp final_ready\n','    mov $51, #$00\n    jmp poly_complete\n')):
            broken=source().replace(before,after,1);self.assertNotEqual(broken,source())
            with self.assertRaises(ValueError):gate_native(PROBE,bank(),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_forged_controls_audio_edges_envelopes_and_metadata_fail(self):
        for mutation in ('mode','final-mode','duration','volume','pulses','retrigger','attack','release','tail','restore','FF'):
            r=copy.deepcopy(self.reports['continue-note-63-8'])
            if mutation=='mode':r['short_continue_mode']=0
            elif mutation=='final-mode':r['final_mode']=1
            elif mutation=='duration':r['events'][8]['duration']=16
            elif mutation=='volume':r['keyons'][5]['volumes'][0]=[7,7]
            elif mutation=='pulses':r['keyons'][5]['pending_pulses'][0]=5
            elif mutation=='retrigger':r['envelopes'][2]['retrigger_half_cycle']=0
            elif mutation=='attack':r['envelopes'][-1]['attack_zero']=False
            elif mutation=='release':r['keyoffs'][-1]['cause']=0
            elif mutation=='tail':r['final_tail_pcm']['nonzero_frames']=1
            elif mutation=='restore':r['restore_equal']=False
            elif mutation=='FF':control_writes(r)[0]['value']=4
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(r)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve();unittest.main(argv=[sys.argv[0],*rest])
