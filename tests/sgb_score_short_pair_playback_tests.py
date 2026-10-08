#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Physical consecutive short-note admission, release and lifecycle checks."""
import argparse,copy,hashlib
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from build_sgb_score_short_pair import build,source
from build_sgb_score_short_continue import build as prior_build
from build_sgb_score_short_pair_fixture import bank,CASES,ARTICULATIONS
from build_sgb_score_short_continue_fixture import bank as prior_bank
from check_sgb_score_short_pair_playback import native,validate,identity
from check_sgb_score_short_continue_playback import validate as prior_validate,SCHEMA as PRIOR_SCHEMA
from check_sgb_score_polygate_reference import native as gate_native
from check_sgb_score_final_playback import observation,control_writes
from check_sgb_score_gate_reference import PULSES
from build_sgb_prototype import assemble
PROBE=None


class ShortPairPlaybackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.reports={f'{c}-{a}':native(PROBE,bank(a,c)) for c in CASES for a in ARTICULATIONS}

    def test_reproducibility_and_unchanged_general_gate_table(self):
        self.assertEqual(build(),build());self.assertEqual(len(build()),4096)
        self.assertEqual(hashlib.sha256(build()).hexdigest(),'b27faaa73cb996f6b6fcc0a3bf48160ead49c90ff268c78a14a9e5e615c78e45')
        self.assertEqual(hashlib.sha256(prior_build()).hexdigest(),'c0a890f508c30169591ccfcb865113b4198c31af2ef6960fa03256180cbaef70')
        for a in ARTICULATIONS:self.assertNotIn((96,a,4),PULSES)

    def test_frozen_voice_retrigger_and_both_short_gate_releases(self):
        for r in self.reports.values():
            c,a,d=identity(r);self.assertEqual(d,4)
            old,new=r['envelopes'][2],r['envelopes'][6]
            self.assertEqual(old['off_half_cycle'],0);self.assertEqual(old['retrigger_half_cycle'],new['on_half_cycle'])
            self.assertGreater(r['frozen_peer_checks'][0],300000)
            self.assertTrue(new['attack_zero']);self.assertEqual(new['peak'],127);self.assertTrue(new['release_zero'])
            self.assertEqual(r['keyons'][4]['pending_pulses'][0],5)
            if c=='continue-note':
                following=r['envelopes'][-1];self.assertTrue(following['attack_zero']);self.assertEqual(following['peak'],127);self.assertTrue(following['release_zero'])
                self.assertEqual(r['keyons'][5]['pending_pulses'][0],5)
                self.assertLess(new['off_half_cycle'],following['on_half_cycle']);self.assertLess(following['off_half_cycle'],control_writes(r)[0]['half_cycle'])
            gates,held=observation(r,allow_retrigger=True);self.assertEqual(held,[])
            self.assertEqual([(g['onset_index'],g['voice']) for g in gates],[(0,2),(0,3),(1,3),(2,3),(3,3),(4,2)]+([(5,2)] if c=='continue-note' else []))
            self.assertTrue(all(g['cause']==1 for g in gates))

    def test_deferred_pitch_volume_control_and_following_rest(self):
        for r in self.reports.values():
            for i in (6,8):
                e=r['events'][i];end=r['events'][i+1]['half_cycle'];ws=[w for w in r['voice_writes'] if e['half_cycle']<=w['half_cycle']<end]
                wanted=[] if e['opcode']==0xC9 else [(34,164 if i==6 else 8),(35,6 if i==6 else 7),(32,7 if i==6 else 1),(33,7 if i==6 else 1)]
                self.assertEqual([(w['address'],w['value']) for w in ws],wanted)
            for e in r['keyons'][4:]:
                self.assertEqual(e['held_mask'],0);ws=[w for w in r['key_writes'] if w['half_cycle']<=e['half_cycle']][-3:]
                self.assertEqual([(w['address'],w['value']) for w in ws],[(92,0),(92,0),(76,4)])

    def test_final_silence_and_full_reset_save_load_source_cache(self):
        for r in self.reports.values():
            self.assertEqual(r['end_tick'],72);self.assertEqual(r['short_pair_mode'],1);self.assertEqual(r['final_mode'],0)
            self.assertTrue(r['reset_equal']);self.assertTrue(r['restore_equal']);self.assertTrue(r['source_unmodified']);self.assertTrue(r['cache_guards_equal'])
            self.assertEqual(r['final_observation_half_cycles'],600000);self.assertEqual(r['final_tail_pcm']['nonzero_frames'],0);self.assertEqual(r['final_env_end'],0)
            self.assertEqual([(w['address'],w['value']) for w in control_writes(r)],[(92,255),(92,0),(76,0)])

    def test_provisional_second_short_note_requires_full_rehearsal_shape(self):
        for offset,value in ((0x6DD,8),(0x6BD,8),(0x6DF,0xA1),(0x6E4,0xA0),(0x6C2,8),(0x6C3,127),(0x6C4,0xA0),(0x2FC,63),(0x2FB,4)):
            b=bytearray(bank());b[offset]=value;r=native(PROBE,bytes(b))
            self.assertEqual(r['status'],226);self.assertEqual(r['key_writes'],[]);self.assertEqual(r['pcm']['nonzero_frames'],0)
        for c in CASES:
            b=bytearray(bank(case=c));b[0x6C2:0x6C6]=bytes(4);r=native(PROBE,bytes(b));self.assertEqual(r['status'],226)
        b=bytearray(bank());b[0x6C5:0x6C9]=bytes((4,63,0xC9,0));self.assertEqual(native(PROBE,bytes(b))['status'],226)

    def test_predecessor_rejection_and_previous_continuation_profiles_survive(self):
        for c in CASES:
            self.assertEqual(gate_native(PROBE,bank(case=c),builder=prior_build,validator=validate,output_bound=131072)['status'],226)
            for a in ARTICULATIONS:
                for d in (8,16):
                    r=gate_native(PROBE,prior_bank(a,d,c),builder=build,validator=lambda r:prior_validate({**r,'schema':PRIOR_SCHEMA}),output_bound=131072)
                    self.assertEqual(r['short_pair_mode'],0)

    def test_short_slot_counter_setup_and_completion_faults_fail(self):
        broken=source().replace('    cmp a, #$82\n    bne short_pair_slot_bad\n','    cmp a, #$84\n    bne short_pair_slot_bad\n',1)
        rejected=gate_native(PROBE,bank(),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)
        self.assertEqual(rejected['status'],226);self.assertEqual(rejected['key_writes'],[])
        for before,after in (('    mov $76, #$05\n','    mov $76, #$0f\n'),('short_continue_voice:\n    call duet_voice\n    jmp pending_mix\n','short_continue_voice:\n    call pending_mix\n    jmp duet_voice\n'),('    mov $51, #$00\n    jmp final_ready\n','    mov $51, #$00\n    jmp poly_complete\n')):
            broken=source().replace(before,after,1);self.assertNotEqual(broken,source())
            with self.assertRaises(ValueError):gate_native(PROBE,bank(),builder=lambda:bytes(assemble(broken,'spc',0x0800)),validator=validate,output_bound=131072)

    def test_forged_mode_gate_attack_release_audio_and_metadata_fail(self):
        for mutation in ('mode','duration','pulses','volume','retrigger','attack','release','tail','restore','FF'):
            r=copy.deepcopy(self.reports['continue-note-63'])
            if mutation=='mode':r['short_pair_mode']=0
            elif mutation=='duration':r['events'][8]['duration']=8
            elif mutation=='pulses':r['keyons'][5]['pending_pulses'][0]=4
            elif mutation=='volume':r['keyons'][5]['volumes'][0]=[7,7]
            elif mutation=='retrigger':r['envelopes'][2]['retrigger_half_cycle']=0
            elif mutation=='attack':r['envelopes'][-1]['attack_zero']=False
            elif mutation=='release':r['keyoffs'][-1]['cause']=0
            elif mutation=='tail':r['final_tail_pcm']['nonzero_frames']=1
            elif mutation=='restore':r['restore_equal']=False
            elif mutation=='FF':control_writes(r)[0]['value']=4
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(r)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--probe',type=Path,required=True);args,rest=p.parse_known_args();PROBE=args.probe.resolve();unittest.main(argv=[sys.argv[0],*rest])
